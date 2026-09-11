#include "rose.h"

class LoopParallelizer : public AstSimpleProcessing {
protected:
    virtual void visit(SgNode* node) {
        SgForStatement* forLoop = isSgForStatement(node);
        
        if (forLoop != NULL) {
            SgNode* parent = forLoop->get_parent();
            while (parent != NULL && !isSgFunctionDeclaration(parent)) {
                if (isSgForStatement(parent)) return; 
                parent = parent->get_parent();
            }

            bool hasFunctionCall = (SageInterface::querySubTree<SgFunctionCallExp>(forLoop).size() > 0);
            
            bool hasDependency = false;
            std::vector<SgPntrArrRefExp*> arrayRefs = SageInterface::querySubTree<SgPntrArrRefExp>(forLoop);
            for (auto ref : arrayRefs) {
                std::vector<SgBinaryOp*> mathOps = SageInterface::querySubTree<SgBinaryOp>(ref->get_rhs_operand());
                if (mathOps.size() > 0) {
                    hasDependency = true; 
                    break;
                }
            }

            if (!hasFunctionCall && !hasDependency) {
                SageInterface::attachArbitraryText(forLoop, "#pragma omp parallel for\n", PreprocessingInfo::before);
            }
        }
    }
};

int main(int argc, char** argv) {
    SgProject* project = frontend(argc, argv);
    LoopParallelizer traversal;
    traversal.traverseInputFiles(project, preorder);
    return backend(project);
}
